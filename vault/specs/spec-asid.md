---
id: spec-asid
type: spec
title: "asid.tla"
models: [sub-kernel-asid]
pins: [inv-i31]
cfgs:
  - "asid.cfg -- clean: NoActiveAlias + NoStaleTLB + ActiveClaimed + CurrentGenClaimed"
  - "asid_buggy_rollover_steals_active.cfg -- the rollover hands out a live CPU's ASID: NoActiveAlias"
  - "asid_buggy_fast_no_regen.cfg -- the fast path skips the generation check: NoActiveAlias"
  - "asid_buggy_fast_no_flush_check.cfg -- the fast path skips the pending-flush check: NoStaleTLB"
  - "asid_buggy_no_flush_pending.cfg -- the rollover omits the flush obligation: NoStaleTLB"
  - "asid_buggy_reserve_value_only.cfg -- an ownerless reservation reclaim: NoActiveAlias (the audit's own F1)"
gate: "any change to arch/arm64/asid.c or to the context-switch pre-hook that calls it"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

Written **model-first** — TLC-green before a line of the allocator existed —
because a generation rollover racing a context switch is the classic subtle
failure of this allocator family, and the consequence of getting it wrong is
silent cross-Proc memory corruption with no signal at either end.

CPUs and Procs, a global generation, a per-CPU active slot, a per-CPU
reservation with an **owner**, a per-CPU pending-flush flag, and an abstract
per-CPU cached translation. A switch is `FastSwitch` (generation matches, the
slot is live, publish and go) or `SlowSwitch` (claim, or roll the generation and
reserve every live ASID before re-offering the bitmap).

**Deliberately beneath the model:**

- the **hardware TLB**. `CacheTranslation` is an abstract per-CPU token, not a
  cache with entries, sets, or an invalidation scope. Whether the local
  invalidate sequence covers what the architecture requires is answered in the
  ARM ARM and in prose, not here;
- the **bitmap** — claiming is a set membership, not a linear scan with a
  round-robin hint, so the search's own bounds are outside;
- the **8-versus-16-bit width**, and with it the whole question of how often a
  rollover happens. The model rolls because its space is tiny by construction;
- **generation wraparound.** Generations are bounded so the model explores
  rollovers, but the arithmetic of a 64-bit counter wrapping is not proven —
  it is unreachable in practice, which is a different claim;
- the **kproc bypass**. A kernel thread never enters the allocator; that gate is
  at the caller and is not modeled as a state.

## Action-site map

Every action lives in `arch/arm64/asid.c`.

| Action | Site |
|---|---|
| `FastSwitch(c, p)` | `asid_resolve` — the lockless first half: the two guards (`old_active != 0 && gen_match(cid)`) and the compare-exchange publish into `g_active_asids[cpu]`, which fails against a concurrent rollover |
| `SlowSwitch(c, p)` | `asid_resolve` — the second half under `g_asid_lock`: `new_context` when the generation is stale, then the pending local flush, then the publish |
| the rollover branch | `new_context`'s bitmap-full arm: bump the generation, then `flush_context` — clear the bitmap, exchange every active slot to zero, preserve it (or the idle CPU's existing reservation) into `reserved_asids`, re-claim it, set `flush_pending` |
| the reservation reclaim | `check_update_reserved` — the **full-context-id** compare that makes a reservation owned rather than merely valued |
| `Deschedule(c)` | no site: descheduling leaves the active slot set, and the slot becomes reclaimable only through the rollover's reserve pass |
| `CacheTranslation(c)` | no site — the hardware TLB fill. Cleared by `asid_local_tlb_flush`, consumed off the `flush_pending` bit on the slow path |

| Invariant | Obligation |
|---|---|
| `NoActiveAlias` | [[inv-i31]] clause 1 — two CPUs active on one ASID implies the same Proc. Threads of one Proc sharing an ASID is the correct case, not a violation |
| `NoStaleTLB` | clause 3 — a running Proc's cached translation for its own ASID is either empty or its own, never another address space's |
| `ActiveClaimed` | clause 2 — every active ASID is claimed in the current generation's bitmap, so the claim path cannot hand it out again |
| `CurrentGenClaimed` | the fast path's soundness premise: a current-generation Proc's ASID is claimed, so reusing it can never collide with a fresh claim |

## The five counterexamples

Each is one way to violate [[inv-i31]], and together they are the argument for
why the fast path has two guards rather than one:

`rollover_steals_active` is the headline bug — the rollover clears the bitmap
and re-offers an ASID a CPU is running on.

`fast_no_regen` and `fast_no_flush_check` remove one fast-path guard each.
Removing the generation check is the obvious bug. **Removing the pending-flush
check is the interesting one**: the generation still matches, the ASID is
genuinely the Proc's own, nothing in the allocator is aliased — and the CPU runs
against TLB entries cached under the previous generation. It violates
`NoStaleTLB` rather than `NoActiveAlias`, which is exactly why the invariant is
stated in three clauses.

`no_flush_pending` is its rollover-side twin: the flush obligation never gets
armed.

`reserve_value_only` is **the audit's own finding, and it was a bug in this
model rather than in the code**. The reservation was originally modeled as a
value with no owner, which lets a Proc reclaim any CPU's reserved ASID that
happens to hold the same number. The implementation never had it — it compares
the full context id — but the model would not have caught a version that did.
The fix carries the owner alongside the value, and required raising the
configuration bound to at least four Procs for the counterexample to be
reachable at all.

That last point is the durable lesson: a specification that is green because its
bounds are too small to reach the bug is indistinguishable from one that is
green because the bug is absent.
