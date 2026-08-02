---
id: lock-asid
type: lock
title: "g_asid_lock — the rolling-ASID allocator guard"
kind: spin
orders-before: []
guards: "the ASID claim bitmap, the global generation counter's writers, the per-CPU active/reserved/flush_pending arrays, and the rollover"
created: 2026-08-02
updated: 2026-08-02
---
## Discipline

A single global spinlock (IRQ-saving) over the whole ASID allocator, taken only
on the **slow path** of a context switch — when the running Proc's stored
generation is stale, or when this CPU owes a post-rollover local TLB flush. The
fast path takes nothing.

It is a **true leaf**: it acquires no other lock, and everything under it is
bitmap arithmetic, per-CPU array writes, and a CPU-local `tlbi vmalle1`. Order
is `runqueue lock -> g_asid_lock`, because the context-switch pre-hook runs with
the run-queue lock already held; nothing anywhere reaches for a run-queue lock
while holding this, so the order is acyclic by inspection of a very small file.

**The generation counter is read outside the lock, so every access to it is
atomic even under the lock.** That asymmetry is deliberate and is easy to undo by
accident: the lock serializes *writers*, but the fast path's generation-match
test loads `g_asid_generation` locklessly, so a plain non-atomic store under the
lock would be a data race against a reader that is not required to hold it. The
same reasoning covers the per-CPU active slots, which the rollover exchanges
atomically precisely so a concurrent fast path observes the zeroing.

Held with interrupts off and for a bounded, small span — the longest operation
is a linear scan of the claim bitmap, which is 255 or 65535 bits wide.

Callers: exactly one, the context-switch ASID pre-hook. See [[sub-kernel-asid]].
