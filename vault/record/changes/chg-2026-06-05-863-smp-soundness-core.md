---
id: chg-2026-06-05-863-smp-soundness-core
type: chg
title: "#863: the pinned in-tree idle retires the off-tree boot idle (closes #860)"
date: 2026-06-05
arc: arc-deep-smp-review
commits: ["4d292f35", "ddd13eff"]
touched: [sub-kernel-sched-smp]
established: []
closed: []
opened: [seam-sparse-mpidr]
mirrors-checked: []
depth: rich
---
## What

The SMP redesign's soundness core. Three changes that are one change:

1. **Every CPU's idle is an ordinary in-tree, `cpu_pinned` thread.** It
   lives in `run_tree[IDLE]`, is dispatched by ordinary `pick_next`, and
   is skipped by `try_steal` because it is pinned. cpu0's idle gets its
   own dedicated guard-paged BSS stack (`g_bootcpu_idle_stack`, #867),
   symmetric with a secondary's.
2. **The boot-CPU deadlock-path dispatch is deleted**, along with the
   off-tree `g_bootcpu_idle` pointer and the racy guard around it.
3. **`idle_in_wfi` becomes accurate** (F7): `sched()` sets it to
   `(next == cs->idle)` at every switch, so a CPU running stolen work no
   longer advertises itself as idle to peers.

Plus #868, pulled forward: cpu0 attaches `IPI_RESCHED` on its own banked
redistributor, so a peer's notify wakes it immediately instead of
leaving it to its next timer tick.

## Why the shape matters more than the fix

`cpu_pinned` is a **single clean predicate replacing an exception**. The
old gate was `kstack_base != NULL && cand != g_bootcpu_idle` — and the
special case in that conjunction *was* #860: `g_bootcpu_idle` had a real
kernel stack, so the `kstack_base` half did not exclude it, and leaking
it into a tree made it stealable. A peer would then build exception
frames on a stack cpu0 still owned.

The consequence worth naming: because a non-idle thread being current
implies its CPU's idle is in the tree, `pick_next` can never come back
empty, and the old "deadlock" extinction becomes **structurally
unreachable** rather than merely unused. It is kept as a loud failure
for the boot window before the idle is installed.

## Model-first

This is the chunk [[spec-sched-alpha]] was written *for* — the target
architecture modelled before it was built, with `IdleStaysHome` and
`IdleAvailable` as the two properties the design turns on. Its diagnostic
sibling [[spec-sched-oncpu]] had already reproduced #860 and compared
this fix against the alternative (keep the special case, guard it).
That comparison happened at the model level, before either was written.

## Also landed here

The `per_cpu_main` assertion that the PSCI context id equals
MPIDR.Aff0 — the only thing standing between a cluster-MPIDR board and
silent per-CPU slot aliasing, since a bounds check cannot detect
aliasing. [[seam-sparse-mpidr]].
