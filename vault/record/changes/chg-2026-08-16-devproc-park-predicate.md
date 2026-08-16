---
id: chg-2026-08-16-devproc-park-predicate
type: chg
title: "A correct model does not guarantee a correct encoding of its states"
date: 2026-08-16
arc: arc-vault
commits: []
touched: [sub-kernel-devproc]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The debug surface's park predicate reported *parked* about a thread that was
about to run, and the finding is unusually rich in transferable shape — five
separable lessons from one defect.

## The stale registration, and why the waker cannot fix it

The predicate tested two things: the thread is registered on its debug
rendezvous, and it is not on a processor. Both hold on a thread **leaving** the
park.

The reason lives in another subsystem and is itself correct: the waker sets the
thread runnable and queues it but deliberately does **not** clear the blocked-on
registration, because only the owning thread may clear it — so the group-
terminate cascade can read it under the wait lock. Correct there, and it leaves a
window here where the registration is stale.

**A field whose clearing is deliberately deferred is a field no reader may treat
as current.** Nothing at the reading site said so.

## The ambiguity, not the race, is what made it fatal

A stop issued while a prior start's wake was still undispatched found the stale
park, the blocking wait returned immediately, and the read landed after dispatch
but before the thread re-parked. The stopped conjunction was then false, and the
caller got a bare denial.

**Indistinguishable from a real authorization refusal.** So the debugger treated
a transient as fatal, and the supervising process exited non-zero — a machine
failure from a condition that would have resolved on the next poll.

The race is one boot in eighty. **The single return value is what made it
unrecoverable**, and that is this surface's stated posture — everything is one
value, and the caller distinguishes by which operation it attempted — meeting a
state that is genuinely temporary. That posture is defensible for denials and
not-founds; it is not defensible for "not yet".

Visible only under hardware-accelerated virtualization, because the faster
processor widens the undispatched window. Substrate deciding observability again,
the second time in this sweep.

## Three properties, and only together do they make it the right fix

The repair adds the thread's own state as a third term. Any one property alone
would have produced a *working* fix:

- **It only narrows.** The term can turn a true into a false and never the
  reverse, so nothing becomes newly readable. On a privilege surface that is the
  only direction a change can move and remain safe **by construction rather than
  by argument** — and it is checkable without reasoning about the race at all.
- **It converges.** A runnable peer is dispatched, re-checks its condition and
  re-registers as sleeping, so the poll terminates instead of spinning on a state
  that never settles.
- **It composes.** A job-parked thread is also sleeping, so a debugger stopping
  an already-job-stopped target still reads fully-stopped at once.

Worth separating them because a fix that merely *works* satisfies the test and
leaves the other two properties to chance.

## A discriminator you set yourself cannot discriminate for you

The stepping path needed no change: it polls the full conjunction, whose
stop-requested term already rejects the stale window.

A stop caller has no such discriminator — **because it sets that flag itself
before waiting.** So the term that serves both callers had to be the one neither
of them writes.

That generalizes past this file. When two callers share a predicate and one of
them establishes part of it, that part is inert for that caller, and a fix built
on it silently covers only half the surface.

## The model was right and the encoding was not

No specification changed. The model has an **abstract** parked state; the defect
lived in the implementation's **two-field encoding** of it, which admitted a
configuration the model does not have.

The fix moved the implementation toward the model, so the model stayed the gate.

**A correct model does not guarantee a correct encoding of its states.** The gap
between an abstract predicate and the concrete fields standing in for it is a
place defects live, and it is **invisible from the model's side** — model
checking cannot find a state its own abstraction cannot express. This project
suspended its specification-first discipline for most surfaces; this is a case
where the specification was present, correct, exhaustively checked, and silent.

## Two probes, blind to each other in both directions

The decision is extracted as a **pure function** of the three inputs, assertable
without constructing a thread, with the walk above it as plumbing.

Both layers are probed separately, and the justification is exact rather than
ceremonial: **dropping the term fails only the pure assertion, and hardcoding the
argument at the call site fails only the walk, with every pure assertion still
green.** Neither probe can see the other layer's sabotage.

That is the discrimination standard this project keeps arriving at, applied
before anything went wrong rather than after — and it is the cheapest place to
apply it, because extracting the decision is what makes both probes possible.
