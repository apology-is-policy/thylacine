---
id: inv-i18
type: inv
title: "I-18 — IPIs from CPU A to CPU B are processed in send order"
number: I-18
guards: [sub-kernel-sched-smp]
validated-by: [spec-scheduler, gate-smp]
strength: spec
created: 2026-08-01
updated: 2026-08-01
---
## Statement

Inter-processor interrupts sent from one CPU to another are processed in
the order they were sent. Per *ordered pair*: nothing is claimed about
interleaving between different senders.

## Enforcement

By the hardware, plus a deliberately trivial handler.

Thylacine sends exactly one IPI at v1.0: `IPI_RESCHED` (SGI 0), via
`gic_send_ipi`. The GIC delivers SGIs to a target's redistributor and the
target takes them at its next unmasked instruction boundary; a pending
SGI is coalesced rather than queued, which is *stronger* than ordering —
two sends collapse to one delivery, and the receiver's action is
idempotent.

The receiver's action is `ipi_resched_handler`: increment a per-CPU
counter and return. It carries no payload and makes no decision. The
actual work happens afterward, when `preempt_check_irq` runs on the
IRQ-return path and finds `need_resched` set — which is *state*, not
message content. That separation is what makes ordering a non-problem in
practice: the IPI is a doorbell, and the message is a flag the sender
already published.

Three senders exist:

- `sched_notify_idle_peer` — wake *any* announced-idle peer to come
  steal. Stops on the first send (waking several is a thundering herd
  where only one gets the work).
- `sched_notify_cpu` — wake a *specific* target after a cross-CPU
  placement. Always paired with `need_resched_set(target)`, which is the
  correctness half; the IPI is only promptness.
- `smp_resched_others` — broadcast to every CPU but self, used by the
  death cascade to trap peers running at EL0 into the kernel so they
  reach their die-check. At most `ncpus - 1` sends on a rare path.

`gic_send_ipi` bounds-checks its target, so an out-of-range or offline
index is a quiet no-op — at `-smp 1` the broadcast sends nothing.

## Validation

[[spec-scheduler]] models per-(src,dst) IPI queues explicitly and checks
`IPIOrdering`; `scheduler_buggy_ipi.cfg` sets `BUGGY_IPI_ORDER` and
produces the counterexample. The model also bounds in-flight IPIs
(`MaxIPIs`) so the state space stays finite. [[gate-smp]] is the
empirical backstop, and `smp.ipi_resched_smoke` proves delivery works at
all.

**blind-to:** the model treats the queue as the ordering authority; the
real GIC *coalesces* pending SGIs rather than queuing them. That
divergence is safe only because the handler is idempotent and carries no
payload — the moment an IPI conveys information beyond "look at your
flags", the model and the hardware stop agreeing and this invariant needs
re-deriving from the GIC's actual guarantees rather than from
`scheduler.tla`.
