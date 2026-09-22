---
id: chg-2026-09-22-poll-preemption-point
type: chg
title: "poll crosses a preemption point each re-loop: the masked CPU gets a window the noise cannot close"
date: 2026-09-22
arc: arc-boosty
commits: ["b8b27f1d", "1f14b6c5"]
touched: [sub-kernel-poll, sub-kernel-sched, spec-poll, spec-poll-cpu]
established: [spec-poll-cpu]
closed: [fnd-b0poll-r5-f1, fnd-b0poll-r6-s1, fnd-b0poll-r7-f2, fnd-b0poll-r7-f3, fnd-b0poll-r7-s4]
opened: [fnd-b0poll-r7-f1]
mirrors-checked: [abi-boot-banner]
depth: rich
created: 2026-09-22
---
`kernel/sched.c` gains `sched_preempt_point()`, and poll's re-arm loop crosses
it on every non-terminal pass. It extincts if a counted lock is held (an IRQ
handler runs on THIS thread's kernel stack during the window and may take
locks), holds `preempt_count` so `preempt_check_irq` defers the switch rather
than taking it inside the window (#360), saves `daif`, unmasks IRQ across an
`isb`, restores the caller's mask, and then consumes a deferred `need_resched`
with `sched()` -- which `sched_yield_hint` alone does not read, and the
EL0-return preempt is a whole syscall away.

The `isb` widens the window rather than guaranteeing it, and the claim is
REPEATED interruptibility rather than per-pass delivery
([[fnd-b0poll-r7-s4]]).

Round 5's spin budget and 1 ms backoff are deleted along with
`g_poll_spin_budget_ns` and `poll_spin_budget_set_for_test`, so round 6's F1
(a test-only setter shipping ungated) dissolves with them. The point is skipped
on a TIMEDOUT pass, which is terminal and needs no bound, and which the model's
`FinalSample` path does not route through `atpoint` either.

`specs/poll.tla` gains `atpoint` / `Point` and `IrqLatencyBounded`;
`specs/poll_cpu.tla` is NEW and carries the claim `poll.tla` structurally
cannot ([[fnd-b0poll-r7-f2]]). `sched.preempt_point_takes_a_pending_irq`
witnesses the window with a masked control ([[fnd-b0poll-r7-f3]]).

It is a STOPGAP by [[dec-2026-09-22-point-now-model-next]]: when syscall bodies
run with interrupts on (ARCH 8.1), it is deleted.
