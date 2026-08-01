---
id: chg-2026-06-11-rw11-wake-preemption
type: chg
title: "RW-11 SA-1b: wake-preemption, the syscall-return preempt point, and a realized INTERACTIVE band"
date: 2026-06-11
arc: arc-holotype-rw
commits: ["fb5e776c"]
touched: [sub-kernel-sched]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
## What

Closed the empirically-pinned **6 ms slice cliff**: a newly-runnable
higher-priority thread waited up to a full slice for the next
tick-driven preempt, because the wake path never requested a reschedule
on the *same* CPU — only the cross-CPU branch did ([[fnd-866-r1-f1]],
a week earlier, had fixed the other half of the same gap).

Three additive parts:

1. **The mechanism** — `sched_wake_preempts` as a pure policy
   (an idle yields to anything; a strictly higher band preempts; same
   band stays EEVDF-fair), plus the `ready_on` hook that sets
   `need_resched(self)` when the wakee outranks the running thread,
   decided under the target's lock where `current_thread()` and
   `cs->idle` are stable.
2. **A syscall-return preempt point** — `preempt_check_irq` at the SVC
   tail, *before* the die-check, mirroring the IRQ-from-EL0 order.
   Syscalls run IRQ-masked, so without it a wake during a syscall waits
   for the next EL0 tick.
3. **A realized INTERACTIVE band** — `sched_mark_interactive` promotes a
   USER thread NORMAL → INTERACTIVE, sticky and one-way, from exactly two
   callers (`kobj_irq_wait`, `devcons_read`), each enforcing its own
   trust gate so the set stays narrow.

## The alternatives

A granularity-guarded same-band preempt (Linux EEVDF's answer) was
throughput-friendlier but leaves the same-band fresh-interrupt tail — the
exact case the IRQ benchmark phase-locks onto. Unconditional same-band
preempt met every latency cell at the cost of more switches. Band
realization is what ARCH §8.3 already specified, and it is what the user
voted for.

## What it does not do

It does not make [[inv-i17]] a bound — it removes one specific
unbounded-looking wait. And it makes ARCH §8.3's "no aging across bands
at v1.0" **load-bearing** rather than theoretical: with INTERACTIVE
realized, a CPU-bound INTERACTIVE thread can starve NORMAL. Bounded in
practice because the realized set is trusted and mostly blocked; the
general CPU-DoS bound is [[inv-i32]], not the scheduler.
