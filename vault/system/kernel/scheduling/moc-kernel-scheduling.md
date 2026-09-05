---
id: moc-kernel-scheduling
type: moc
title: "Kernel scheduling: dispatch, the SMP protocol, and the wait/wake primitive"
parent: moc-kernel
created: 2026-08-01
updated: 2026-08-01
---
What decides which Thread runs on which CPU, and what happens to a Thread
that stops running. [[moc-kernel-execution]] owns the Thread's *identity*
and its *death*; this area owns its *motion*.

Three layers, in the order a Thread meets them:

- **Dispatch** — per-CPU run trees, three fixed-priority bands, a
  virtual-deadline sort, and the preemption machinery that takes the CPU
  back.
- **The SMP protocol** — how a Thread crosses CPUs without two of them
  ever writing its context. This is the layer the tree's bug history
  lives in.
- **The wait/wake primitive** — `sleep`/`tsleep`/`wakeup` over a Rendez.
  Everything that blocks in Thylacine blocks here.

The area's defining property is that **almost nothing in it is atomic**.
A context switch is a multi-step sequence with a lock handed across two
threads; a steal claims a victim on one CPU and loads it on another; a
sleep registers, drops two locks, and only then yields. Every one of the
lineage's bugs (#788, #806, #860, #104, #866-F1, #363) lives in a window
between two of those steps, and the model that missed them
([[spec-scheduler]]) missed them precisely because it modelled the steps
as atomic. That is why the redesign is gated by a *second* model
([[spec-sched-alpha]]) that refuses to.

## Children

- [[sub-kernel-sched]] — dispatch: `struct CpuSched`, the bands and the
  `vd_t` sort, `sched()`'s yield-vs-block contract, placement, the tick /
  `need_resched` / `preempt_check_irq` chain, and the per-thread
  preempt count that makes a spinlock hold non-preemptible.
- [[sub-kernel-sched-smp]] — the SMP protocol: the `on_cpu` claim, the
  cross-thread `pending_release_lock` handoff, work-stealing, the pinned
  in-tree idle, the tickless idle park, and secondary bring-up.
- [[sub-kernel-rendez]] — the wait/wake primitive: the single-waiter
  Rendez, `sleep`/`tsleep`/`wakeup`, the global timer-wait list, and the
  death and stop detours that every blocking site inherits.

## Cross-cutting

- Invariants: [[inv-i8]] (every runnable thread eventually runs) ·
  [[inv-i17]] (the EEVDF latency bound — a design target, not an
  as-built guarantee) · [[inv-i18]] (IPIs are processed in send order) ·
  [[inv-i21]] (a Thread runs on at most one CPU; the kernel is uniformly
  EL1h) · [[inv-i9]] (no lost wake — the wait/wake half lives here, the
  death half in [[sub-kernel-death]]).
- Specs — the largest family in the tree, and the reason it is large:
  [[spec-scheduler]] (the original; proved blind to the bug class) ·
  [[spec-sched-oncpu]] (the diagnostic that reproduced #860) ·
  [[spec-sched-alpha]] (the redesign's gating model) ·
  [[spec-sched-ctxsw]] (I-21's EL1h consistency) ·
  [[spec-sched-tickless]] (the idle arm-race) ·
  [[spec-sched-rebalance]] (the busy-side overload kick) ·
  [[spec-tsleep]] (the third wake source).
- Locks, outermost first: [[lock-wait]] → [[lock-timerwait]] →
  [[lock-rendez]] → [[lock-runq]]. [[lock-proc-table]] sits above all
  four. The chain is total and acyclic; every reversal in it has been a
  bug.
- Arcs: [[arc-phase2-lifecycle]] (where dispatch came from) ·
  [[arc-deep-smp-review]] (the redesign — #857/#860/#863/#864/#866) ·
  [[arc-tickless-idle]] (NO_HZ_IDLE and the work-conservation
  regression it caused) · [[arc-go-build]] (#359/#360, #33 — what a real
  toolchain workload broke) · [[arc-holotype-rw]] (#811, RW-11) ·
  [[arc-go-ide]] and [[arc-pty]] (the two stop owners that park inside
  `sleep`).
- Adjacent areas: [[moc-kernel-execution]] (`thread_free` must see a
  Thread off every run tree and off every CPU) · [[moc-kernel-ninep]]
  (the elected 9P reader is the one sleeper whose unwind is deferred to
  a frame boundary).
