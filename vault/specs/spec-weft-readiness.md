---
id: spec-weft-readiness
type: spec
title: "weft_readiness.tla"
models: [sub-kernel-weft]
pins: [inv-i9]
cfgs:
  - "weft_readiness.cfg -- clean: NoLostReadyWake, ParkedIsArmed, SeenBoundedByPosted"
  - "weft_readiness_liveness.cfg -- EventuallyDrained: a posted edge always reaches the consumer"
  - "weft_readiness_buggy_lost_wake.cfg -- BUGGY_OBSERVE_BEFORE_ARM: re-read the counter before publishing park intent (NoLostReadyWake counterexample)"
gate: "any change to the arm/observe ordering, the memory ordering on either word pair, or the single-writer split"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

Two participants in different Procs, communicating through two pairs of words in
a shared page. The producer posts readiness edges; the consumer either notices
them while running or parks and is woken.

This is the store-buffer litmus test, promoted to an invariant. Each side does a
sequentially-consistent store followed by a sequentially-consistent load, on
*opposite* words: the producer bumps the edge counter then reads the park
intent; the consumer publishes its intent then re-reads the counter. In the
global order at least one must see the other's write, so an edge arriving inside
the parking window cannot be lost by both.

It earns a module of its own rather than a place in [[spec-weft]] because it is
about memory ordering, not about lifetimes or authority — and because it is the
only wake in the tree with no lock anywhere in it.

## What it pins

- **`NoLostReadyWake`** — [[inv-i9]] in its shared-memory form. Every other
  instance of this invariant in the tree is register-then-observe under a lock:
  the waiter holds the object's lock across enqueueing itself and re-checking the
  condition, and the producer takes the same lock. Here there is no lock and no
  kernel involvement on the fast path, so the ordering *is* the proof.
- **`ParkedIsArmed`** — a parked consumer is always visible as armed, which is
  what makes the producer's decision to wake correct.
- **`SeenBoundedByPosted`** — the consumer's cursor never runs ahead of what was
  actually posted.

The buggy configuration is the natural mistake and the reason the model exists:
re-read the counter *before* publishing the intent. It reads as equivalent and
is not, and the counterexample is short.

## What it cannot see

The wake itself. The model says the producer must wake a parked consumer; the
mechanism for doing so is a rendezvous outside this abstraction, and the
single-writer discipline the implementation keeps — the producer *never* writes
the consumer's words, even to wake it — is a code-level property the model has no
vocabulary for.

The cache-line padding is likewise invisible. Correctness does not depend on it;
performance does, and the whole point of the mechanism is performance.

The userspace mirror of these primitives lives in the native client library and
is validated by construction — same sequence, same orderings — rather than by
being modelled separately. That mirroring is what makes this a *cross-Proc*
protocol rather than a kernel one, and it is the least mechanically checked part
of the arrangement.

## Binding

`specs/SPEC-TO-CODE.md::weft_readiness.tla`. ProducerEdge ↔ the mask store, the
sequentially-consistent counter bump, and the intent load; ConsumerPark ↔ the
intent publish followed by the sequentially-consistent counter re-read and the
un-arm on a raced edge; ConsumerProcess ↔ the acquire-load fast path.
