---
id: fnd-b0poll-r7-s4
type: fnd
title: "The isb sabotage PASSES: the prose had upgraded the point into a per-pass delivery guarantee the architecture never gives, while the model stated it correctly"
round: adt-b0poll-r7
severity: P3
status: documented
surface: [sub-kernel-poll, sub-kernel-sched]
threatens: [inv-i9]
fixed-by: chg-2026-09-22-poll-preemption-point
regression: "sched.preempt_point_takes_a_pending_irq (nodaifclr RED; noisb is a NON-discriminating control)"
created: 2026-09-22
---

## Prosecution

Found by running the sabotages [[fnd-b0poll-r7-f3]]'s fix asked for. The
witness test's assertion read "drop the daifclr, OR the isb, and this fails".
With the `isb` removed and the `daifclr` kept, the suite is **1615/1615 PASS**
-- a sabotage that passes.

The claim, not the test, is what is wrong. A direct write to PSTATE.DAIF takes
effect with no barrier: Linux's `__daif_local_irq_enable` is a bare `msr
daifclr, #3`, and only the ICC_PMR_EL1 priority-mask path needs `pmr_sync()`.
What the `isb` buys is a synchronization event BETWEEN the two MSRs instead of
leaving them adjacent, which is the shape arm64 KVM uses to transiently unmask.

Following it down found the same overclaim in three further places. The
architecture gives NO bound on when a pending unmasked interrupt is taken, only
that it is taken in finite time -- so no single crossing can be guaranteed to
deliver, and "every interrupt pending at that moment is taken" was never true
of any implementation of this. What the point buys is that the CPU is
REPEATEDLY interruptible, which is exactly what `poll_cpu.tla` already stated
(`Open` is "at a point or idle", never "an interrupt was taken here"). **The
formal statement was the conservative one; four pieces of English had drifted
past it in the same direction.**

## Disposition

FIXED at `0434a4bc`: the emitted instruction sequence is unchanged (the `isb`
stays, for the reason above and at that cost), and the claim is corrected in
`sched.c`'s contract comment, the test's assertion message and a comment saying
why `noisb` is not asserted, ARCH 23.3 (including "interrupt delay under noise
is one pass", now stated as the masked SPAN being one pass -- the distinction
from round 5's bound on the masked RATE), and the two dossiers that repeated
it. The sabotage stays in the script labelled a NON-discriminating control: if
it ever goes red, the window got narrower than the architecture allows.
