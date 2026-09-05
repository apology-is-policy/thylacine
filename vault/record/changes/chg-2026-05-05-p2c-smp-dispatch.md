---
id: chg-2026-05-05-p2c-smp-dispatch
type: chg
title: "P2-Cd/Ce/Cf: per-CPU run trees, work-stealing, and the on_cpu handoff"
date: 2026-05-05
arc: arc-phase2-lifecycle
commits: ["a604cd7d", "fa6ded07", "6cdfc8ab"]
touched: [sub-kernel-sched, sub-kernel-sched-smp]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
## What

SMP dispatch, in the three steps that introduced every window the next
year of bugs lived in.

**P2-Cd** — the global run tree becomes `struct CpuSched g_cpu_sched[]`,
one slot per CPU, each with its own tree, its own `vd_counter`, and its
own idle thread. `sched_init` takes a `cpu_idx`.

**P2-Ce** — work-stealing (`try_steal`: trylock peers, take one
non-pinned runnable thread, rebase its `vd_t` into the stealer's clock)
and the **`finish_task_switch` handoff**: `prev` acquires the run-queue
lock and the *resuming* thread releases it via
`cs->pending_release_lock`, because after `cpu_switch_context` the
resuming thread may be on a different CPU than the one whose lock is
held.

**P2-Cf** — `on_cpu`: the cross-CPU "this context is claimed" flag,
cleared by the destination CPU's resume path once the switch is complete,
so `wakeup` can spin on it rather than ready a thread off a half-saved
context.

## What it did not do

It kept the boot CPU's *second* dispatch route — an off-tree
`g_bootcpu_idle` reached through a global pointer on a deadlock path.
That special case is #860's root cause and was retired only at
[[chg-2026-06-05-863-smp-soundness-core]].

And [[spec-scheduler]] modelled `Steal` as a single atomic transfer with
no `on_cpu` at all — so the model stayed green through every bug these
three commits made possible. That gap is the whole reason
[[spec-sched-oncpu]] and [[spec-sched-alpha]] exist.
