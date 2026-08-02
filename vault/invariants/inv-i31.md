---
id: inv-i31
type: inv
title: "I-31 — no two CPUs run different address spaces under one ASID"
number: I-31
guards: [sub-kernel-asid]
validated-by: [spec-asid, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

Three clauses, and the third is what makes the first two reachable:

1. **No aliasing.** No two CPUs concurrently run distinct user address spaces
   sharing an ASID within the same generation.
2. **No stealing.** A generation rollover never reassigns an ASID that is
   `active` or `reserved` on any CPU — a running CPU is never yanked.
3. **No stale reuse.** Every context switch installs a valid current-generation
   ASID *before* the TTBR0 write.

## Why the consequence is severe

An ASID tags TLB entries. If two address spaces are live under one tag, the TLB
cannot disambiguate them, and a translation cached for one Proc satisfies a
lookup by the other. That is not a fault, a trap, or a denial of service — it is
**one Proc silently reading and writing another Proc's memory**, with no signal
at either end.

There is no defense downstream. Nothing above the MMU can detect that a
translation was wrong; the memory simply is what the wrong page table said. So
the invariant has to hold at the allocator, and it has to hold against
concurrency, because a rollover on one CPU races every other CPU's context
switch by construction.

## Why it is stated as three clauses

Clause 1 is the property that matters. Clauses 2 and 3 are the two ways to
violate it, stated separately because they are enforced by different mechanisms
and can be broken independently.

**Clause 2 is the classic rolling-ASID bug.** When the ASID space fills, the
allocator bumps a generation and clears its claim bitmap — every ASID becomes
free again. But some of them are *in use right now on other CPUs*. Handing one
of those out is clause 1's violation, arrived at by bookkeeping rather than by
any obviously dangerous operation.

**Clause 3 is the subtler one**, and it is about the TLB rather than the
allocator. A rollover's broadcast invalidations do not cover speculation in the
rollover window, so a CPU can hold entries cached under the previous
generation's assignment. Running a recycled ASID against those entries is
clause 1's violation with no aliasing anywhere in the allocator's state — the
alias is in the cache.

## Enforcement

**Generation above value.** A Proc's identifier is one word: the ASID value in
the low bits, a global generation in the high bits. Generation zero is "never
assigned" and always mismatches, so a zeroed structure is correct by default
rather than by an initializer.

**Publish before you use.** A CPU writes the identifier it is about to run into
its own per-CPU active slot *before* the TTBR0 write, so a concurrent rollover
observes it as live. This is clause 3's first half and it is why the publish is
a compare-exchange rather than a store: it must fail if a rollover intervened.

**Reserve at rollover.** The rollover walks every CPU, atomically exchanges its
active slot to zero, preserves that value into a reserved slot, and re-claims it
in the fresh bitmap before offering the bitmap to anyone. That is clause 2,
directly. The exchange-to-zero doubles as the interlock: a concurrent fast path
reading a zeroed slot fails its compare-exchange and is forced onto the slow
path.

**A per-CPU flush obligation.** The rollover sets a pending-flush flag for every
CPU; that CPU issues a *local* TLB invalidate before its next publish. This is
clause 3's second half, and it is why the fast path has **two** guards rather
than one: generation-match alone would let a CPU with a pending flush proceed.

**Reservations are owned, not merely valued.** Reclaiming a reservation compares
the *whole* identifier — generation and value — so a Proc reclaims only its own.
Comparing the value alone lets one Proc take another live Proc's ASID, which is
clause 1 by a different route.

## Validation

[[spec-asid]] was written and TLC-green **before** the implementation, on the
reasoning that this is the classic subtle failure of this allocator family and
exactly what machine-checked exploration is for. It carries five counterexample
configurations, one per way to break it: the rollover steals an active ASID; the
fast path skips the generation check; the fast path skips the pending-flush
check; the rollover omits the flush obligation; the reservation is reclaimed by
value rather than by owner.

The focused audit that followed found a bug **in the model** rather than the
code — the reservation modeled without an owner — which is the outcome
model-first is meant to produce, and which is where the fifth configuration came
from.

[[gate-smp]] is the empirical backstop.

**blind-to:** the model reasons about the allocator's state, not about the
hardware TLB. The stale-entry clause is represented as an abstract per-CPU
obligation; whether the local invalidate sequence actually covers what the
architecture says it must is an ARM-ARM question answered in prose. And the
model bounds generations, so it explores rollovers but not the arithmetic of a
generation counter wrapping a 64-bit word — which is unreachable in practice
rather than proven.
