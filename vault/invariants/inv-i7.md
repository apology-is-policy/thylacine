---
id: inv-i7
type: inv
title: "I-7 — a memory object lives until both ways of reaching it are gone"
number: I-7
guards: [sub-kernel-burrow]
validated-by: [spec-burrow, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

A memory object's pages are alive **if and only if** at least one of its two
reference counts is above zero: the count of open handles naming it, or the
count of installed mappings of it.

Both directions are obligations. Freeing while either count is positive is a
use-after-free. Not freeing when both have reached zero is a leak. The model
checks it as a biconditional for exactly that reason — a one-directional
statement would be satisfied by never freeing anything.

## Why two counts

Because there are two independent ways to reach the object, and neither
subsumes the other.

A Proc can map a region and then close the handle it mapped from — the mapping
is live, the handle is gone, and the pages must stay. A Proc can hold a handle
to a region it has never mapped — nothing is mapped, the pages must still stay.
The two reachabilities are genuinely orthogonal, so a single counter would have
to conflate them, and any conflation gets one of the two cases wrong.

The invariant is therefore not "refcount the object" — that is the trivial
part. It is "**there are two different kinds of reference and the object
outlives both**."

## The sharp edge is the decision, not the counts

The failure mode this invariant guards is not a lost increment. It is that the
zero-zero test appears in **two places** — once where a handle is dropped, once
where a mapping is dropped — and asks the same question of shared state.

Unsynchronized, two Threads dropping the last handle and the last mapping
concurrently can both observe zero-zero and free twice, or interleave their
decrements such that neither observes it and the object leaks. The counts being
individually atomic would not fix it; the *decision* is what has to be atomic
with the decrement that enables it.

So the enforcement is: decrement and compute the free decision under the
object's own lock, carry the decision out as a value, and free outside the
lock. Exactly one racing dropper carries a true out.

Freeing outside the lock is not an optimization. The free path reaches the page
allocator, the hardware-object refcounts, and a filesystem clunk; holding the
object's lock across it would nest those locks under it on the free path while
the mapping path nests them outside — the cycle.

## What "alive" means per backing

The invariant is stated over "pages" but the object has six backing types and
they do not all own pages the same way. Three hold one contiguous run; two hold
a sparse per-page array where a null slot is normal rather than freed; two hold
a *reference to a separately refcounted object* rather than pages at all.

The invariant survives this because it is about the **object's** lifetime, not
the backing's: when both counts reach zero the object releases whatever it
holds, and a foreign backing's own refcount then decides when the underlying
resource dies. The type-dispatched release is where that translation happens,
and each arm carries its own double-release guard.

## Enforcement

**The decision under the lock, the free outside it** — the mechanism above.

**A resurrection guard on every acquire.** Both counts at zero means the object
is already dead; taking a reference would revive a dead identity, which is the
use-after-free with the causality reversed. Every acquire path checks it under
the same lock.

**A liveness check per backing type on mapping acquire** — reading the field
that type actually uses, since a null `pages` is a use-after-free for a
contiguous backing but a wholly normal state for a sparse one.

**A magic sentinel at offset zero**, so the slab allocator's own freelist write
clobbers it on free. An operation on a freed object then sees a wrong magic and
extincts with a clear diagnostic instead of proceeding into recycled memory.

**A re-assertion inside the free path** that both counts are zero. Redundant
with its caller by construction, and kept as the tripwire for a future caller
that frees without asking.

## Validation

[[spec-burrow]] pins it as `NoUseAfterFree`, with three counterexample
configurations that are the three ways to get it wrong: free when the handle
count reaches zero, free when the mapping count reaches zero, never free. The
first two violate it prematurely, the third violates it by delay — which is
what makes the biconditional worth stating.

[[gate-smp]] is the empirical backstop for the race the model does not reach.

**blind-to:** the model is single-stepping — its transitions are atomic — so it
proves the *arithmetic* of the dual count and not the mutual exclusion that
makes the arithmetic hold on real hardware. The lock discipline is prose and
audit, not machine-checked. That gap is precisely where the real bug lived: the
counts were correct as written and wrong as executed.

The cross-Proc case, where one object is reachable from two address spaces, is
also below the model — it is the same arithmetic with the references originating
in different Procs, and rests on the argument that the count does not care where
a reference came from.
