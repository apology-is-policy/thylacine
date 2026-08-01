---
id: arc-deep-smp-review
type: arc
title: "The deep SMP review: model the mechanism, then redesign"
status: active
design: ["docs/ARCHITECTURE.md 8.4"]
chunks:
  - chg-2026-06-05-863-smp-soundness-core
  - chg-2026-06-05-864-hmp-foundation
follow-ons: [seam-hmp-push, seam-sparse-mpidr]
created: 2026-08-01
---
## Goal

Stop point-patching the SMP scheduler and fix the thing that made the
patches necessary: the model was blind to the bug class.

## What preceded it

A lineage of individually-fixed SMP bugs — #788, #806, #807, #808, #860 —
each a window inside the multi-step context switch, and each fixed on its
own terms. Through all of them [[spec-scheduler]] stayed green, because
it models `Steal` as a **single atomic transfer** and has no `on_cpu`
variable at all. It proved the high-level state machine sound under an
atomicity assumption the implementation does not satisfy.

#857 was the tell that something was wrong with the *instruments* too: an
"smp8 `cons.*` flake" that turned out to be `sched_runnable_count`
counting the per-CPU in-tree idles as pending work. Never a kernel fault
— a measurement bug producing a phantom backlog and racing the quiescence
assertions.

## The method

Model-first, in three modules rather than one extension:

1. [[spec-sched-oncpu]] — the **diagnostic**. Re-introduce exactly what
   `scheduler.tla` abstracts away (`on_cpu`, the per-CPU lock held across
   the switch, the boot CPU's second dispatch route) and #860 reproduces
   as a trace. Then model both candidate fixes and compare them.
2. [[spec-sched-alpha]] — the **gating model** of the fix that won: every
   CPU has a pinned, in-tree idle; no special case at all. Placement is
   deliberately non-deterministic, so the safety result holds for any
   policy.
3. The impl, checked against it.

## Chunks

- **#863** ([[chg-2026-06-05-863-smp-soundness-core]]) — the pinned
  in-tree idle retires the off-tree boot idle; `idle_in_wfi` accuracy;
  cpu0 becomes a full IPI peer. #860 closes by construction.
- **#864** ([[chg-2026-06-05-864-hmp-foundation]]) — placement policy
  split from enqueue mechanism, per-CPU capacity, per-task util,
  `balance()`'s push-capable shape. Inert on every uniform target.
- **#866** — the formal audit of both, closed clean at [[adt-866-r1]].

## What the arc leaves open

The push half of HMP and its empirical tuning ([[seam-hmp-push]]), and
the dense-MPIDR assumption the redesign made explicit and loud rather
than silent ([[seam-sparse-mpidr]]). Both are deliberate: the first is
unverifiable without heterogeneous hardware, the second fails closed.

## The lesson worth carrying

A green model is evidence about the model. `scheduler.tla` was not wrong
— it was *not about the thing that was broken*, and it was retained
unmodified for what it does cover. Keeping the diagnostic module beside
the gating one is what makes that concrete instead of a maxim.
