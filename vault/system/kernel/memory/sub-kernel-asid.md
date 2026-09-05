---
id: sub-kernel-asid
type: sub
parent: moc-kernel-memory
title: "The rolling-ASID allocator — a recycled cache, not a per-Proc allocation"
code: ["arch/arm64/asid.c", "arch/arm64/asid.h"]
audit: hard
guarded-by: [inv-i31]
validated-by: [spec-asid, gate-smp]
locks: [lock-asid]
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

An address-space identifier tags TLB entries so a context switch need not flush
the whole TLB. The hardware field is 8 or 16 bits wide, which makes it a
**scarce resource that must be recycled** — and the recycling is where the
subtlety lives.

The predecessor design allocated one ASID per Proc, permanently, at creation.
It worked until the (ASID-space + 1)th concurrent Proc, at which point the
kernel extincted: an unprivileged whole-system denial of service reachable by
anyone who could fork. This layer replaces it with the Linux arm64 rolling
model — a global **generation** counter above the ASID value, so the space is
reused wholesale rather than parcelled out — and thereby removes exhaustion as a
concept rather than raising a limit.

## Contract

Three entry points and three diagnostics, in 273 lines.

`asid_init()` is called once after the MMU is up. It reads the hardware width,
sizes the bitmap, zeroes the per-CPU state, and stamps the generation to 1.
Calling it twice extincts.

`asid_resolve(context_id, cpu)` is the whole allocator. Given a pointer to a
Proc's stored `context_id` and the logical CPU it is about to run on, it returns
the **hardware ASID value** (not the full context id) for the caller to compose
into TTBR0, updating the stored context id and the per-CPU state as needed.

Its preconditions are load-bearing and unchecked except for one:

- **IRQs masked, on a stable CPU.** The function publishes into
  `active_asids[cpu]`; if the caller could migrate between choosing `cpu` and
  the publish, it would publish into a slot naming a CPU it is not on. The
  context-switch pre-hook satisfies this by holding the run-queue lock.
- **Never for kproc.** A kernel thread has `pgtable_root == 0` and uses the
  kernel TTBR0 at ASID 0, bypassing the allocator entirely. The caller gates on
  that, not this function.
- **`cpu < DTB_MAX_CPUS`** — the one checked precondition, and it extincts
  rather than returning, because the per-CPU arrays are fixed-size and an
  out-of-range index would corrupt adjacent BSS.

`asid_bits()`, `asid_generation_now()`, `asid_rollover_count()` are lockless
diagnostic snapshots.

## Mechanism

A `context_id` is one `u64`: the hardware ASID value in the low bits, the
generation in the high bits. **Generation 0 is "never assigned"** and always
mismatches, which is what makes a zeroed Proc structure correct by default
rather than by an initializer.

### The fast path is lockless and rests on two guards

```
if (old_active != 0 && gen_match(cid) && cmpxchg(&active[cpu], old_active, cid))
        return cid & val_mask;
```

Both conditions are necessary, and the second is the one that looks optional:

1. **Generation match** — the Proc's stored generation equals the global one, so
   its ASID value is still valid this generation.
2. **This CPU's active slot is non-zero** — no rollover has zeroed it since this
   CPU last ran. A rollover *exchanges every active slot to zero*, so a zero
   slot means "this CPU owes a local TLB flush." Skipping this check runs a CPU
   over stale TLB entries across a rollover.

The compare-exchange is the third guard folded into the publish: it fails if a
rollover zeroed the slot between the read and here, dropping through to the slow
path. The publish happens **before** the caller writes TTBR0, so a concurrent
rollover sees this CPU as active and preserves its ASID rather than reassigning
it.

### The slow path claims, or rolls

Under [[lock-asid]], `new_context` tries three things in order, and the ordering
is the reclaim policy:

1. **The reservation re-stamp.** If the Proc's old context id is still reserved
   on some CPU, re-stamp that reservation to the current generation and keep the
   same ASID value.
2. **The still-free case.** If the old ASID value is unclaimed this generation,
   claim it and keep it. Both of these preserve ASID locality across a
   rollover — the same Proc tends to get the same value.
3. **A fresh claim** from the bitmap, round-robin from a hint.

If the bitmap is full, it **rolls over**: bump the generation, then
`flush_context`.

### The rollover is where the safety obligation lives

`flush_context` clears the bitmap and then, for every CPU:

- **Exchanges** the active slot to zero, atomically. This is the interlock — it
  is what makes a concurrent fast path's compare-exchange fail.
- Preserves that value (or, if the CPU was idle, its existing reservation) into
  the reserved slot and **re-claims it in the fresh bitmap**. This is the
  no-steal obligation: a running CPU's ASID is never handed to anyone else.
- Sets `flush_pending`, so that CPU issues a local `tlbi vmalle1` before it next
  runs anything.

The idle-CPU carry-over — `if (a == 0) a = reserved[i]` — exists for
**back-to-back rollovers**: a CPU that has not run since the last rollover has a
zero active slot but a live reservation, and dropping it would let the second
rollover steal an ASID the first one promised.

There is an `extinction` if no ASID is free *after* a rollover. It is provably
unreachable: the rollover reserves at most one value per CPU, and the smallest
ASID space (255) exceeds `DTB_MAX_CPUS` (8) by a wide margin.

### Why the local flush is needed at all

The broadcast inner-shareable invalidations issued elsewhere do not cover
speculation that occurred in the rollover window. The per-CPU `flush_pending`
flag is consumed on the slow path, under the lock, immediately before the
publish — so a CPU cannot run a recycled ASID against entries it cached under
the previous generation.

## Data structures

All file-static, no dynamic allocation:

| | |
|---|---|
| `g_asid_generation` | the global counter; high bits of every context id. Written under the lock, **read locklessly** — hence atomic on every access |
| `g_asid_map[]` | the claim bitmap, sized for the 16-bit ceiling (8 KiB BSS) regardless of the detected width |
| `g_active_asids[]` | per-CPU: "this CPU is running this context id"; zero means none |
| `g_reserved_asids[]` | per-CPU: preserved across a rollover |
| `g_flush_pending[]` | per-CPU: owes a local TLB flush |

The bitmap is statically sized for the widest case and only its low
`1 << width` bits are used — 8 KiB of BSS to avoid a runtime allocation in a
path that runs under a spinlock with interrupts off.

## Concurrency

See [[lock-asid]]. The essential asymmetry: **the lock serializes writers, but
the generation counter and the active slots have lockless readers**, so every
access to them is `__atomic_*` even inside the critical section. A plain store
under the lock would be a data race against a reader that is not required to
hold it.

The reservation is **owned, not merely valued**. `check_update_reserved`
compares the *full* context id, not just the ASID value, so a Proc reclaims only
a reservation that is actually its own. The value-only form is the spec's F1
counterexample: it lets one Proc reclaim another live Proc's ASID, which is
precisely the aliasing [[inv-i31]] forbids.

## Invariants enforced

[[inv-i31]] in full — no two CPUs concurrently run distinct address spaces
sharing an ASID. Locally that decomposes into: the rollover never steals an
active or reserved value; the fast path publishes before the TTBR0 write; and a
CPU with a pending flush cannot take the fast path.

## Error paths

There are no error returns. Every failure is an `extinction`, on the argument
that each represents structural corruption rather than a condition a caller
could handle: init called twice, resolve before init, a CPU index out of range,
no free ASID after a rollover.

## Performance

The fast path is one relaxed load, one comparison, and a compare-exchange — no
lock, no barrier, no flush. The slow path runs once per Proc per generation.
Rollover frequency is set by the width: with 16-bit ASIDs (65535 values) it is
rare enough to be a non-event; with 8-bit (255) it is the reason the local-flush
machinery has to be correct rather than merely present.

## Prosecution

- **The rollover-versus-switch race** is the whole surface. Any change to the
  order of {bump generation, clear bitmap, exchange active slots, set
  flush_pending} must preserve: a CPU whose slot is zeroed cannot complete a
  fast path, and its value is re-claimed before the bitmap is offered to anyone.
- **Both fast-path guards** must survive. Dropping the active-slot check is a
  named counterexample, not a hypothetical.
- **The full-context-id compare** in the reservation reclaim. Narrowing it to
  the value is the F1 bug.
- **Lock order.** `g_asid_lock` is a leaf under the run-queue lock. Adding any
  acquisition under it — a print, an allocation, a statistics update touching
  another lock — creates the cycle.
- **kproc's bypass** stays gated on `pgtable_root != 0` at the caller.

## Seams

- **[[seam-sparse-mpidr]]** — the `cpu < DTB_MAX_CPUS` bound is a fail-fast, but
  the dense-logical-index assumption it rests on is shared with the whole
  scheduler and tracked there.
- **[[seam-hwcap-boot-cpu-only]]** is the same shape one layer over: the bitmap
  and generation are sized from the **boot CPU's** width. The architecture
  requires a uniform ASID width across all PEs, so this is sound by
  specification rather than by measurement — but it is an assumption, recorded
  here because a violating system would silently mis-size the space rather than
  fail.

## Caveats

**The function this layer is named after does not exist.** Four scripture
locations — the audit-trigger row in `CLAUDE.md`, the same row in
`ARCHITECTURE.md`, the architecture's own wiring paragraph, and the action-site
map in `SPEC-TO-CODE.md` — all name `asid_check_and_switch`. The entry point is
`asid_resolve`, and has been since it landed; nothing named
`asid_check_and_switch` appears anywhere in the tree. The
`sched_install_asid_ttbr0` pre-hook is the caller.

That matters more than a typo because of *where* it appears: the action-site map
exists so a reader can get from a model action to the code that realizes it, and
the audit-trigger row is the prosecution list an auditor works from. Both point
at a name that cannot be grepped. Tracked as a main-track fix.

The map's fast/slow split is also mis-attributed — it maps the fast path to the
named function and the slow path to `new_context`, where in fact **both halves
are `asid_resolve`** and `new_context` is the claim-or-roll helper the slow half
calls under the lock.

## Provenance

The design landed model-first: [[spec-asid]] was written and TLC-green before
this code, on the reasoning that a generation-rollover race is the classic
subtle failure of this allocator family and exactly the class machine-checked
exploration is for. The focused audit that followed found a bug **in the spec**
rather than the implementation — the reservation modeled as a value without an
owner — which is the outcome model-first is supposed to produce.
