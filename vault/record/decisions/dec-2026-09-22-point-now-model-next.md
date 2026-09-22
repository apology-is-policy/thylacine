---
id: dec-2026-09-22-point-now-model-next
type: dec
title: "Point now, model next: a preemption point in poll today, ARCH 8.1 built as written before the browser kernel work"
date: 2026-09-22
status: standing
decided-by: user-vote
affects: [sub-kernel-poll, sub-kernel-sched, sub-kernel-syscall-abi]
created: 2026-09-22
---
## Observation

Thylacine runs syscall bodies IRQ-masked from EL0 exception entry to return, so
any syscall that LOOPS holds its CPU's interrupts for as long as the loop runs.
poll is such a syscall, and driving its loop takes no privilege
([[fnd-b0poll-r5-f1]]). A sleep does not bound it: the bound is per-thread
while the obligation is the CPU's, and even a per-CPU backoff bounds the RATE
of masked passes and never the masked SPAN ([[fnd-b0poll-r6-s1]]).

Two things were established before the choice was put. First, the masking is an
ACCIDENT, not a design: Phase 0 deferred kernel PREEMPTION to Phase 7, P3-Ec
wired the SVC path and never unmasked, and the deferral of preemption was
therefore BUILT as "interrupts off" -- a different property. The corrective
Phase-7 deliverable never reached a status doc. Second, a research battery
found the construct is standard practice with a name (seL4's
`preemptionPoint()`, Fiasco.OC's `Proc::preemption_point`, NOVA's, arm64
Linux KVM's transient unmask), and that no peer kernel shares Thylacine's pair
of traits -- masked syscall bodies AND blocking loops inside them. The
heritage does not either: 9front's `dosyscall` calls `spllo()`.

## Decision

Both halves, in order.

**Now:** a preemption point in poll's loop -- `sched_preempt_point`, asserting
no counted lock is held, holding `preempt_count` so the #360 gate defers the
switch, unmasking and re-masking around an `isb`, then consuming a deferred
`need_resched`. Unconditional, so its bound is per-PASS and composes across
pollers where a per-thread budget did not.

**Next, as its own spec-first audited kernel chunk, BEFORE the F3-F9 browser
kernel work:** ARCH 8.1 built as written -- syscall bodies with interrupts ON,
still non-preemptible. It DELETES the point.

The point is a stopgap by decision, not by drift, and that is recorded at the
point itself so the deletion is not forgotten. What the stopgap does not cover
is stated with it: it bounds the number of masked spans a poller takes, not the
length of one ([[fnd-b0poll-r7-f1]]), and `pipe_block_locked` /
`chan_role_acquire` share the loop's shape and are left for the 8.1 chunk.
