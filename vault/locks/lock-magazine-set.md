---
id: lock-magazine-set
type: lock
title: "percpu_data.lock — one CPU's magazine sets"
kind: spin-irqsave
guards: "one CPU's two magazine stacks (count + entries[]) at orders 0 and 9"
orders-before: [lock-buddy-zone]
created: 2026-10-21
updated: 2026-10-21
---
## Discipline

One per CPU slot (`g_percpu[NCPUS]`, `mm/magazines.c`). The owner takes
its own set's lock inside the IRQ mask on every `mag_alloc` and
`mag_free`: the mask comes first because it is what makes `my_cpu()` name
the set whose lock is taken. Any CPU may take a set's lock to drain it
(`magazines_drain_all`, one set at a time, irqsave). Uncontended in
steady state; contended only by a drain, or by two CPUs a clustered SoC
folds onto one slot ([[seam-sparse-mpidr]]).

Order: this lock, then [[lock-buddy-zone]] (refill and drain-on-full call
`buddy_alloc`/`buddy_free` under it). No path holds two magazine locks,
and nothing takes this lock under the zone lock. It is not a leaf
(`spin_leaf_enter`) and does not refuse one.

Heritage: illumos kmem's per-CPU cache takes `cc_lock` on every alloc and
free (Bonwick & Adams 2001); Linux's per-CPU page lists gained a per-CPU
spinlock in 2022 so a peer can drain them without an IPI. Operator vote,
2026-10-21 ([[sub-kernel-mm-phys]]).

## Held across

The pop or push, and a refill or drain-on-full: at most `MAGAZINE_SIZE/2`
buddy calls, each taking and dropping the zone lock. A full drain holds
it across at most `2 * MAGAZINE_SIZE` buddy frees. Never across page-field
initialisation (that runs after release, on a page private to the caller).

## Prosecution

- A peer-CPU touch of `g_percpu` without the set's lock is the defect this
  lock closed: the IRQ mask excludes nothing running on a peer, and a drain
  racing a push or pop hands one page to two owners with every count in
  range, so the #807 count guard stays silent (host model, 2026-10-07:
  7575 double allocations, 0 guard fires).
- The witness is `tools/test-magazines.py`: the actual bodies under TSan,
  three churning CPUs and one drainer; each mutant that drops one side's
  lock must draw a race report.
